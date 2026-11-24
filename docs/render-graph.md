# The render graph

`badger/render_graph.h` is how the renderer records a frame, as Unreal's
render dependency graph is. Each frame it builds a graph: the textures and
buffers the frame uses, created for the frame or imported from outside it,
and passes that say which of them they read and write, and in what state.
Executing the graph works out what the passes leave to it.

| The graph | How |
| --- | --- |
| Culls work nothing uses | A pass is kept when it writes a resource a kept pass reads, writes an imported resource, or is marked `never_cull` (a readback for the CPU). Walking the passes backward, what a kept pass reads is needed of earlier passes, and what it overwrites (a target cleared, or drawn over with `TB_RHI_LOAD_ANY`) is not. |
| Places barriers | Each resource is moved into the state each kept pass declared, in the order passes were added. Shader writes one after another get a barrier between them. Imported resources end in the state the import asked for. |
| Gives memory | A transient texture or buffer gets memory at its first use and gives it back after its last, so a later resource of the same description takes it in the same frame. Memory is kept from frame to frame, in the state it was left in, and released after three frames unused. A transient texture's usage is what its passes' states need, so a description need not list it. |
| Records and submits | Each kept pass inside a debug marker of its name; a pass with color or depth targets inside a pass of the RHI over them. |

```c
tb_render_graph_begin(graph);
const tb_render_graph_texture back = tb_render_graph_import_texture(
    graph, tb_rhi_swap_chain_current(chain), TB_RHI_STATE_PRESENT, TB_RHI_STATE_PRESENT, "back");
const tb_render_graph_texture hdr = tb_render_graph_create_texture(graph, &hdr_desc, "hdr");

tb_render_graph_pass scene = {.name = "scene", .execute = draw_scene, .context = world};
scene.colors[0] = (tb_render_graph_color){.texture = hdr, .load = TB_RHI_LOAD_CLEAR};
scene.color_count = 1;
tb_render_graph_add_pass(graph, &scene);

tb_render_graph_pass tonemap = {.name = "tonemap", .execute = draw_tonemap, .context = post};
tonemap.accesses[0] = (tb_render_graph_access){.texture = hdr, .state = TB_RHI_STATE_SHADER_READ};
tonemap.access_count = 1;
tonemap.colors[0] = (tb_render_graph_color){.texture = back, .load = TB_RHI_LOAD_ANY};
tonemap.color_count = 1;
tb_render_graph_add_pass(graph, &tonemap);

(void)tb_render_graph_execute(graph);
```

A pass's callback records into the command list it is given, and turns
handles into the RHI's with `tb_render_graph_rhi_texture()` and
`tb_render_graph_rhi_buffer()`.

`tb_render_graph_dump()` describes the last frame, one line each, for the
console and tests:

```
pass scene
  memory 0 for hdr
  barrier hdr common -> render_target
pass tonemap
  barrier hdr render_target -> shader_read
  barrier back present -> render_target
final back render_target -> present
```

## Not yet

- Passes are recorded on one thread, into one command list. Recording passes
  in parallel on the job system comes with a renderer that has enough passes
  to need it.
- Memory is shared between resources of the same description, not placed in
  heaps where resources of different descriptions alias.
- Async compute.
