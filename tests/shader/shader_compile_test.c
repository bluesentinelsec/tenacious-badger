/* Compiling a shader's entry point, and its reflection: the stage, the
 * thread group, the values it takes and gives by their semantics, and the
 * bindings, with registers assigned as DXC assigns them. The expected
 * bindings and signatures were read from DXC 1.8.2502's disassembly
 * (`dxc -T ps_6_6 -Fc`) of the same shaders. */
#include "badger/arena.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "tb_test.h"

#define MAIN_PATH "/Engine/Private/Main.hlsl"

typedef struct compiled
{
    tb_arena *arena;
    tb_shader_result result;
    tb_shader_compiled output;
} compiled;

static void compile_text(compiled *out, const char *text, const char *entry_point,
                         tb_shader_stage stage)
{
    tb_shader_file file = {0};
    file.path = MAIN_PATH;
    file.text = text;
    file.length = tb_str_length(text);
    tb_shader_compile_request request = {0};
    request.source.path = MAIN_PATH;
    request.source.files = &file;
    request.source.file_count = 1;
    request.entry_point = entry_point;
    request.stage = stage;
    out->arena = tb_arena_create(0);
    out->result = tb_shader_compile(&request, out->arena, &out->output);
    if (out->output.diagnostic_count > 0)
    {
        TB_TEST_CONTEXT("first diagnostic: %s", out->output.diagnostics[0].message);
    }
}

static void expect_binding(const tb_shader_binding *binding, const char *name,
                           tb_shader_binding_kind kind, unsigned space, unsigned index,
                           unsigned count)
{
    TB_TEST_CONTEXT("binding %s", name);
    TB_TEST_EXPECT_STRING(binding->name, ==, name);
    TB_TEST_EXPECT_UINT(binding->kind, ==, kind);
    TB_TEST_EXPECT_UINT(binding->space, ==, space);
    TB_TEST_EXPECT_UINT(binding->register_index, ==, index);
    TB_TEST_EXPECT_UINT(binding->count, ==, count);
}

static void expect_element(const tb_shader_signature_element *element, const char *semantic,
                           unsigned index, tb_shader_component component, unsigned columns,
                           tb_shader_interpolation interpolation)
{
    TB_TEST_CONTEXT("element %s%u", semantic, index);
    TB_TEST_EXPECT_STRING(element->semantic, ==, semantic);
    TB_TEST_EXPECT_UINT(element->semantic_index, ==, index);
    TB_TEST_EXPECT_UINT(element->component, ==, component);
    TB_TEST_EXPECT_UINT(element->columns, ==, columns);
    TB_TEST_EXPECT_UINT(element->interpolation, ==, interpolation);
}

/* The shader DXC was asked about: what it reports, this reports. */
static const char *const pixel_source =
    "Texture2D unused_tex;\n"
    "Texture2D albedo;\n"
    "Texture2D normals : register(t0);\n"
    "Texture2D extra;\n"
    "SamplerState s;\n"
    "SamplerComparisonState shadow : register(s1); Texture2D<float> depth;\n"
    "RWTexture2D<float4> output;\n"
    "StructuredBuffer<float4> data : register(t2, space1);\n"
    "Texture2D<float> arr[4];\n"
    "cbuffer Frame { float4 tint; float time; };\n"
    "float4 loose;\n"
    "struct PSIn { float4 position : SV_Position; float2 uv : TEXCOORD0; "
    "nointerpolation uint id : ID; float3 normal[2] : NORMAL; };\n"
    "struct PSOut { float4 color : SV_Target0; float4 other : SV_Target2; };\n"
    "PSOut main(PSIn i, bool front : SV_IsFrontFace) {\n"
    "  PSOut o;\n"
    "  o.color = albedo.Sample(s, i.uv) * tint + normals.Sample(s, i.uv) + "
    "extra.Sample(s, i.uv) * time + loose;\n"
    "  o.color += data[i.id] + arr[i.id % 4].Sample(s, i.uv) + depth.SampleCmp(shadow, i.uv, "
    "0.5);\n"
    "  o.other = float4(i.normal[1], front ? 1 : 0);\n"
    "  output[uint2(0, 0)] = o.color;\n"
    "  return o;\n"
    "}\n";

TB_TEST(shader_compile, registers_are_assigned_as_dxc_assigns_them)
{
    compiled out = {0};
    compile_text(&out, pixel_source, "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *const reflection = &out.output.reflection;
    TB_TEST_EXPECT_UINT(reflection->stage, ==, TB_SHADER_STAGE_PIXEL);
    /* What the entry point does not use is not bound; `$Globals` is first,
     * and what has no register takes the lowest free one, in order. */
    TB_TEST_REQUIRE_UINT(reflection->binding_count, ==, 11);
    const tb_shader_binding *const b = reflection->bindings;
    expect_binding(&b[0], "$Globals", TB_SHADER_BINDING_CONSTANT_BUFFER, 0, 0, 1);
    expect_binding(&b[1], "Frame", TB_SHADER_BINDING_CONSTANT_BUFFER, 0, 1, 1);
    expect_binding(&b[2], "s", TB_SHADER_BINDING_SAMPLER, 0, 0, 1);
    expect_binding(&b[3], "shadow", TB_SHADER_BINDING_SAMPLER, 0, 1, 1);
    expect_binding(&b[4], "albedo", TB_SHADER_BINDING_READ, 0, 1, 1);
    expect_binding(&b[5], "normals", TB_SHADER_BINDING_READ, 0, 0, 1);
    expect_binding(&b[6], "extra", TB_SHADER_BINDING_READ, 0, 2, 1);
    expect_binding(&b[7], "depth", TB_SHADER_BINDING_READ, 0, 3, 1);
    expect_binding(&b[8], "data", TB_SHADER_BINDING_READ, 1, 2, 1);
    expect_binding(&b[9], "arr", TB_SHADER_BINDING_READ, 0, 4, 4);
    expect_binding(&b[10], "output", TB_SHADER_BINDING_READ_WRITE, 0, 0, 1);
    TB_TEST_EXPECT_UINT(b[4].shape, ==, TB_SHADER_SHAPE_TEXTURE2D);
    TB_TEST_EXPECT_UINT(b[4].element, ==, TB_SHADER_COMPONENT_FLOAT32);
    TB_TEST_EXPECT_UINT(b[4].element_columns, ==, 4);
    TB_TEST_EXPECT_UINT(b[7].element_columns, ==, 1);
    TB_TEST_EXPECT_UINT(b[8].shape, ==, TB_SHADER_SHAPE_STRUCTURED_BUFFER);
    TB_TEST_EXPECT_UINT(b[0].shape, ==, TB_SHADER_SHAPE_NONE);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, signatures_pass_each_semantic_as_dxc_does)
{
    compiled out = {0};
    compile_text(&out, pixel_source, "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *const reflection = &out.output.reflection;
    /* An array passes one element for each of its elements; a bool passes
     * as a uint; floats interpolate linearly, SV_Position without
     * perspective, and integers not at all. */
    TB_TEST_REQUIRE_UINT(reflection->input_count, ==, 6);
    const tb_shader_signature_element *const in = reflection->inputs;
    expect_element(&in[0], "SV_Position", 0, TB_SHADER_COMPONENT_FLOAT32, 4,
                   TB_SHADER_INTERPOLATION_NOPERSPECTIVE);
    expect_element(&in[1], "TEXCOORD", 0, TB_SHADER_COMPONENT_FLOAT32, 2,
                   TB_SHADER_INTERPOLATION_LINEAR);
    expect_element(&in[2], "ID", 0, TB_SHADER_COMPONENT_UINT32, 1,
                   TB_SHADER_INTERPOLATION_CONSTANT);
    expect_element(&in[3], "NORMAL", 0, TB_SHADER_COMPONENT_FLOAT32, 3,
                   TB_SHADER_INTERPOLATION_LINEAR);
    expect_element(&in[4], "NORMAL", 1, TB_SHADER_COMPONENT_FLOAT32, 3,
                   TB_SHADER_INTERPOLATION_LINEAR);
    expect_element(&in[5], "SV_IsFrontFace", 0, TB_SHADER_COMPONENT_UINT32, 1,
                   TB_SHADER_INTERPOLATION_CONSTANT);
    TB_TEST_REQUIRE_UINT(reflection->output_count, ==, 2);
    expect_element(&reflection->outputs[0], "SV_Target", 0, TB_SHADER_COMPONENT_FLOAT32, 4,
                   TB_SHADER_INTERPOLATION_NONE);
    expect_element(&reflection->outputs[1], "SV_Target", 2, TB_SHADER_COMPONENT_FLOAT32, 4,
                   TB_SHADER_INTERPOLATION_NONE);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, a_compute_shader_has_a_thread_group_and_no_signature)
{
    compiled out = {0};
    compile_text(&out,
                 "RWBuffer<uint> outputs;\n"
                 "[numthreads(8, 4, 2)] void cs(uint3 id : SV_DispatchThreadID) { outputs[id.x] = "
                 "id.y; }",
                 "cs", TB_SHADER_STAGE_COMPUTE);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *const reflection = &out.output.reflection;
    TB_TEST_EXPECT_UINT(reflection->thread_group[0], ==, 8);
    TB_TEST_EXPECT_UINT(reflection->thread_group[1], ==, 4);
    TB_TEST_EXPECT_UINT(reflection->thread_group[2], ==, 2);
    TB_TEST_EXPECT_UINT(reflection->input_count, ==, 0);
    TB_TEST_EXPECT_UINT(reflection->output_count, ==, 0);
    TB_TEST_REQUIRE_UINT(reflection->binding_count, ==, 1);
    expect_binding(&reflection->bindings[0], "outputs", TB_SHADER_BINDING_READ_WRITE, 0, 0, 1);
    TB_TEST_EXPECT_UINT(reflection->bindings[0].shape, ==, TB_SHADER_SHAPE_BUFFER);
    TB_TEST_EXPECT_UINT(reflection->bindings[0].element, ==, TB_SHADER_COMPONENT_UINT32);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, vertex_and_geometry_signatures)
{
    compiled out = {0};
    /* A vertex shader's outputs are not interpolated yet; a matrix passes a
     * row for each semantic index. */
    compile_text(&out,
                 "struct VSOut { float4 position : SV_Position; float3x4 frame : FRAME2; };\n"
                 "VSOut vs(float3 p : POSITION, uint id : SV_VertexID) { VSOut o; "
                 "o.position = float4(p, id); o.frame = 0; return o; }",
                 "vs", TB_SHADER_STAGE_VERTEX);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    const tb_shader_reflection *reflection = &out.output.reflection;
    TB_TEST_REQUIRE_UINT(reflection->input_count, ==, 2);
    expect_element(&reflection->inputs[0], "POSITION", 0, TB_SHADER_COMPONENT_FLOAT32, 3,
                   TB_SHADER_INTERPOLATION_NONE);
    expect_element(&reflection->inputs[1], "SV_VertexID", 0, TB_SHADER_COMPONENT_UINT32, 1,
                   TB_SHADER_INTERPOLATION_NONE);
    TB_TEST_REQUIRE_UINT(reflection->output_count, ==, 4);
    expect_element(&reflection->outputs[1], "FRAME", 2, TB_SHADER_COMPONENT_FLOAT32, 4,
                   TB_SHADER_INTERPOLATION_NONE);
    expect_element(&reflection->outputs[3], "FRAME", 4, TB_SHADER_COMPONENT_FLOAT32, 4,
                   TB_SHADER_INTERPOLATION_NONE);
    tb_arena_destroy(out.arena);

    /* A geometry shader takes its primitive's vertex once and gives its stream's. */
    compile_text(
        &out,
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD; };\n"
        "[maxvertexcount(3)] void gs(triangle V input[3], inout TriangleStream<V> "
        "stream, uint primitive : SV_PrimitiveID) { stream.Append(input[primitive % 3]); }",
        "gs", TB_SHADER_STAGE_GEOMETRY);
    TB_TEST_REQUIRE_UINT(out.result, ==, TB_SHADER_OK);
    reflection = &out.output.reflection;
    TB_TEST_REQUIRE_UINT(reflection->input_count, ==, 3);
    expect_element(&reflection->inputs[1], "TEXCOORD", 0, TB_SHADER_COMPONENT_FLOAT32, 2,
                   TB_SHADER_INTERPOLATION_NONE);
    expect_element(&reflection->inputs[2], "SV_PrimitiveID", 0, TB_SHADER_COMPONENT_UINT32, 1,
                   TB_SHADER_INTERPOLATION_NONE);
    TB_TEST_EXPECT_UINT(reflection->output_count, ==, 2);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, what_cannot_be_compiled_says_why)
{
    compiled out = {0};
    compile_text(&out, "float4 main() : SV_Target { return 1; }", "nope", TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].message, ==,
                          "there is no function 'nope' to run");
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].path, ==, "");
    tb_arena_destroy(out.arena);

    compile_text(&out, "float4 main(float4 x) : SV_Target { return x; }", "main",
                 TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].message, ==,
                          "entry point 'main': 'x' is passed between stages without a semantic");
    tb_arena_destroy(out.arena);

    compile_text(&out,
                 "Texture2D t[]; SamplerState s;\n"
                 "float4 main(float2 uv : UV) : SV_Target { return t[0].Sample(s, uv); }",
                 "main", TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].message, ==,
                          "entry point 'main': 't', an array of resources of no size, needs a "
                          "register");
    tb_arena_destroy(out.arena);

    /* The source's own errors come first, with where they are. */
    compile_text(&out, "float4 main() : SV_Target { return missing; }", "main",
                 TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, >, 0);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].path, ==, MAIN_PATH);
    tb_arena_destroy(out.arena);

    /* A request that names no entry point or no stage is invalid. */
    compile_text(&out, "void main() {}", "", TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_INVALID);
    tb_arena_destroy(out.arena);
    compile_text(&out, "void main() {}", "main", (tb_shader_stage)99);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_INVALID);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_compile, stages_are_named_as_hlsl_names_them)
{
    TB_TEST_EXPECT_STRING(tb_shader_stage_name(TB_SHADER_STAGE_PIXEL), ==, "pixel");
    TB_TEST_EXPECT_STRING(tb_shader_stage_name(TB_SHADER_STAGE_RAY_GENERATION), ==,
                          "raygeneration");
    TB_TEST_EXPECT_STRING(tb_shader_stage_name((tb_shader_stage)99), ==, "unknown stage");
}
